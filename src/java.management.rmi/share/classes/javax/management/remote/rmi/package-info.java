/*
 * Copyright (c) 2002, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

/**
 *    <p>The RMI connector is a connector for the JMX Remote API that
 *      uses RMI to transmit client requests to a remote MBean server.
 *      This package defines the classes that the user of an RMI
 *      connector needs to reference directly, for both the client and
 *      server sides.  It also defines certain classes that the user
 *      will not usually reference directly, but that must be defined so
 *      that different implementations of the RMI connector can
 *      interoperate.</p>
 *
 *    <p>The RMI connector supports the JRMP transport for RMI.</p>
 *
 *    <p>Like most connectors in the JMX Remote API, an RMI connector
 *      usually has an address, which
 *      is a {@link javax.management.remote.JMXServiceURL
 *      JMXServiceURL}.  The protocol part of this address is
 *      <code>rmi</code> for a connector that uses the default RMI
 *      transport (JRMP).</p>
 *
 *    <p>There are two forms for RMI connector addresses:</p>
 *
 *    <ul>
 *      <li>
 *  In the <em>JNDI form</em>, the URL indicates <em>where to find
 *  an RMI stub for the connector</em>.  This RMI stub is a Java
 *  object of type {@link javax.management.remote.rmi.RMIServer
 *  RMIServer} that gives remote access to the connector server.
 *  With this address form, the RMI stub is obtained from an
 *  external directory entry included in the URL.  An external
 *  directory is any directory recognized by {@link javax.naming
 *  JNDI}, typically the RMI registry, LDAP, or COS Naming.
 *
 *      <li>
 *  In the <em>encoded form</em>, the URL directly includes the
 *  information needed to connect to the connector server.  When
 *  using RMI/JRMP, the encoded form is the serialized RMI stub
 *  for the server object, encoded using BASE64 without embedded
 *  newlines.
 *    </ul>
 *
 *    <p>Addresses are covered in more detail below.</p>
 *
 *
 *    <h2>Creating an RMI connector server</h2>
 *
 *    <p>The usual way to create an RMI connector server is to supply an
 *      RMI connector address to the method {@link
 *      javax.management.remote.JMXConnectorServerFactory#newJMXConnectorServer
 *      JMXConnectorServerFactory.newJMXConnectorServer}.  The MBean
 *      server to which the connector server is attached can be
 *      specified as a parameter to that method.  Alternatively, the
 *      connector server can be registered as an MBean in that MBean
 *      server.</p>
 *
 *    <p>An RMI connector server can also be created by constructing an
 *      instance of {@link
 *      javax.management.remote.rmi.RMIConnectorServer
 *      RMIConnectorServer}, explicitly or through the MBean server's
 *      <code>createMBean</code> method.</p>
 *
 *    <h3>Choosing the RMI transport</h3>
 *
 *    <p>You can choose the RMI transport by specifying
 *      <code>rmi</code> in the <code><em>protocol</em></code> part of the
 *      <code>serviceURL</code> when creating the connector server.  You
 *      can also create specialized connector servers by instantiating
 *      an appropriate subclass of {@link
 *      javax.management.remote.rmi.RMIServerImpl RMIServerImpl} and
 *      supplying it to the <code>RMIConnectorServer</code>
 *      constructor.</p>
 *
 *
 *    <h3><a id="servergen">Connector addresses generated by the
 *  server</a></h3>
 *
 *    <p>If the <code>serviceURL</code> you specify has an empty URL
 *      path (after the optional host and port), or if you do not
 *      specify a <code>serviceURL</code>, then the connector server
 *      will fabricate a new <code>JMXServiceURL</code> that clients can
 *      use to connect:</p>
 *
 *    <ul>
 *
 *      <li><p>If the <code>serviceURL</code> looks like:</p>
 *
 *  <pre>
 *  <code>service:jmx:rmi://<em>host</em>:<em>port</em></code>
 *  </pre>
 *
 *  <p>then the connector server will generate an {@link
 *  javax.management.remote.rmi.RMIJRMPServerImpl
 *  RMIJRMPServerImpl} and the returned <code>JMXServiceURL</code>
 *  looks like:</p>
 *
 *  <pre>
 *  <code>service:jmx:rmi://<em>host</em>:<em>port</em>/stub/<em>XXXX</em></code>
 *  </pre>
 *
 *  <p>where <code><em>XXXX</em></code> is the serialized form of the
 *  stub for the generated object, encoded in BASE64 without
 *  newlines.</p>
 *
 *      <li><p>If there is no <code>serviceURL</code>, there must be a
 *  user-provided <code>RMIServerImpl</code>.  The connector server
 *        will generate a <code>JMXServiceURL</code> using the <code>rmi</code>
 *  form.</p>
 *
 *    </ul>
 *
 *    <p>The <code><em>host</em></code> in a user-provided
 *      <code>serviceURL</code> is optional.  If present, it is copied
 *      into the generated <code>JMXServiceURL</code> but otherwise
 *      ignored.  If absent, the generated <code>JXMServiceURL</code>
 *      will have the local host name.</p>
 *
 *    <p>The <code><em>port</em></code> in a user-provided
 *      <code>serviceURL</code> is also optional.  If present, it is
 *      also copied into the generated <code>JMXServiceURL</code>;
 *      otherwise, the generated <code>JMXServiceURL</code> has no port.
 *      For an <code>serviceURL</code> using the <code>rmi</code>
 *      protocol, the <code><em>port</em></code>, if present, indicates
 *      what port the generated remote object should be exported on.  It
 *      has no other effect.</p>
 *
 *    <p>If the user provides an <code>RMIServerImpl</code> rather than a
 *      <code>JMXServiceURL</code>, then the generated
 *      <code>JMXServiceURL</code> will have the local host name in its
 *      <code><em>host</em></code> part and no
 *      <code><em>port</em></code>.</p>
 *
 *
 *    <h3><a id="directory">Connector addresses based on directory
 *  entries</a></h3>
 *
 *    <p>As an alternative to the generated addresses just described,
 *      the <code>serviceURL</code> address supplied when creating a
 *      connector server can specify a <em>directory address</em> in
 *      which to store the provided or generated <code>RMIServer</code>
 *      stub.  This directory address is then used by both client and
 *      server.</p>
 *
 *    <p>In this case, the <code>serviceURL</code> has the following form:</p>
 *
 *    <pre>
 *    <code>service:jmx:rmi://<em>host</em>:<em>port</em>/jndi/<em>jndi-name</em></code>
 *    </pre>
 *
 *    <p>Here, <code><em>jndi-name</em></code> is a string that can be
 *      supplied to {@link javax.naming.InitialContext#bind
 *      javax.naming.InitialContext.bind}.</p>
 *
 *    <p>As usual, the <code><em>host</em></code> and
 *      <code>:<em>port</em></code> can be omitted.</p>
 *
 *    <p>The connector server will generate an
 *      <code>RMIServerImpl</code> based on the protocol
 *      (<code>rmi</code>) and the <code><em>port</em></code> if any.  When
 *      the connector server is started, it will derive a stub from this
 *      object using its {@link
 *      javax.management.remote.rmi.RMIServerImpl#toStub toStub} method
 *      and store the object using the given
 *      <code><em>jndi-name</em></code>.  The properties defined by the
 *      JNDI API are consulted as usual.</p>
 *
 *    <p>For example, if the <code>JMXServiceURL</code> is:
 *
 *      <pre>
 *      <code>service:jmx:rmi://ignoredhost/jndi/rmi://myhost/myname</code>
 *      </pre>
 *
 *      then the connector server will generate an
 *      <code>RMIJRMPServerImpl</code> and store its stub using the JNDI
 *      name
 *
 *      <pre>
 *      <code>rmi://myhost/myname</code>
 *      </pre>
 *
 *      which means entry <code>myname</code> in the RMI registry
 *      running on the default port of host <code>myhost</code>.  Note
 *      that the RMI registry only allows registration from the local
 *      host.  So, in this case, <code>myhost</code> must be the name
 *      (or a name) of the host that the connector server is running
 *      on.
 *
 *    <p>In this <code>JMXServiceURL</code>, the first <code>rmi:</code>
 *      specifies the RMI
 *      connector, while the second <code>rmi:</code> specifies the RMI
 *      registry.
 *
 *    <p>As another example, if the <code>JMXServiceURL</code> is:
 *
 *      <pre>
 *      <code>service:jmx:rmi://ignoredhost/jndi/ldap://dirhost:9999/cn=this,ou=that</code>
 *      </pre>
 *
 *      then the connector server will generate an
 *      <code>RMIJRMPServerImpl</code> and store its stub using the JNDI
 *      name
 *
 *      <pre>
 *      <code>ldap://dirhost:9999/cn=this,ou=that</code>
 *      </pre>
 *
 *      which means entry <code>cn=this,ou=that</code> in the LDAP
 *      directory running on port 9999 of host <code>dirhost</code>.
 *
 *    <p>If the <code>JMXServiceURL</code> is:
 *
 *      <pre>
 *      <code>service:jmx:rmi://ignoredhost/jndi/cn=this,ou=that</code>
 *      </pre>
 *
 *      then the connector server will generate an
 *      <code>RMIJRMPServerImpl</code> and store its stub using the JNDI
 *      name
 *
 *      <pre>
 *      <code>cn=this,ou=that</code>
 *      </pre>
 *
 *      For this case to work, the JNDI API must have been configured
 *      appropriately to supply the information about what directory to
 *      use.
 *
 *    <p>In these examples, the host name <code>ignoredhost</code> is
 *      not used by the connector server or its clients.  It can be
 *      omitted, for example:</p>
 *
 *      <pre>
 *      <code>service:jmx:rmi:///jndi/cn=this,ou=that</code>
 *      </pre>
 *
 *    <p>However, it is good practice to use the name of the host
 *      where the connector server is running.  This is often different
 *      from the name of the directory host.</p>
 *
 *
 *    <h3>Connector server attributes</h3>
 *
 *    <p>When using the default JRMP transport, RMI socket factories can
 *      be specified using the attributes
 *      <code>jmx.remote.rmi.client.socket.factory</code> and
 *      <code>jmx.remote.rmi.server.socket.factory</code> in the
 *      <code>environment</code> given to the
 *      <code>RMIConnectorServer</code> constructor.  The values of these
 *      attributes must be of type {@link
 *      java.rmi.server.RMIClientSocketFactory} and {@link
 *      java.rmi.server.RMIServerSocketFactory}, respectively.  These
 *      factories are used when creating the RMI objects associated with
 *      the connector.</p>
 *
 *    <h2>Creating an RMI connector client</h2>
 *
 *    <p>An RMI connector client is usually constructed using {@link
 *      javax.management.remote.JMXConnectorFactory}, with a
 *      <code>JMXServiceURL</code> that has <code>rmi</code> as its protocol.</p>
 *
 *    <p>If the <code>JMXServiceURL</code> was generated by the server,
 *      as described above under <a href="#servergen">"connector
 *      addresses generated by the server"</a>, then the client will
 *      need to obtain it directly or indirectly from the server.
 *      Typically, the server makes the <code>JMXServiceURL</code>
 *      available by storing it in a file or a lookup service.</p>
 *
 *    <p>If the <code>JMXServiceURL</code> uses the directory syntax, as
 *      described above under <a href="#directory">"connector addresses
 *      based on directory entries"</a>, then the client may obtain it
 *      as just explained, or client and server may both know the
 *      appropriate directory entry to use.  For example, if the
 *      connector server for the Whatsit agent uses the entry
 *      <code>whatsit-agent-connector</code> in the RMI registry on host
 *      <code>myhost</code>, then client and server can both know
 *      that the appropriate <code>JMXServiceURL</code> is:</p>
 *
 *    <pre>
 *    <code>service:jmx:rmi:///jndi/rmi://myhost/whatsit-agent-connector</code>
 *    </pre>
 *
 *    <p>If you have an RMI stub of type {@link
 *      javax.management.remote.rmi.RMIServer RMIServer}, you can
 *      construct an RMI connection directly by using the appropriate
 *      constructor of {@link javax.management.remote.rmi.RMIConnector
 *      RMIConnector}.</p>
 *
 *    <h2>Dynamic code downloading</h2>
 *
 *    <p>If an RMI connector client or server receives from its peer an
 *      instance of a class that it does not know, and if dynamic code
 *      downloading is active for the RMI connection, then the class can
 *      be downloaded from a codebase specified by the peer.
 *      {@extLink rmi_guide Java RMI Guide} explains this in more detail.</p>
 *
 *    @see <a href="http://www.ietf.org/rfc/rfc2045.txt">RFC 2045,
 *    section 6.8, "Base64 Content-Transfer-Encoding"</a>
 *
 *
 *    @since 1.5
 *
 */
package javax.management.remote.rmi;
